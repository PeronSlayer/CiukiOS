"""Kernel32 file endpoint over the production filesystem, without QEMU."""
import os
import signal
from pathlib import Path
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[3]
class Kernel32Files(unittest.TestCase):
    def test_regional_settings_reader(self):
        out=ROOT/'build/host/win32/regional';out.mkdir(parents=True,exist_ok=True)
        program=out/'reader'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
            str(ROOT/'tests/host/win32/regional_io_test.c'),'-o',str(program)],check=True)
        result=subprocess.run([program],capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        print(result.stdout,end='')

    def test_i386_fiber_and_apc_runtime(self):
        from tests.host.win32.adapter import build
        program=build.build_kernel32_runtime(ROOT/'build/host/win32/kernel32-i386')
        result=subprocess.run([program],capture_output=True,text=True,timeout=20)
        if result.returncode==-signal.SIGSYS:
            self.skipTest('Linux i386 adapter blocked by sandbox seccomp (SIGSYS); runtime unqualified')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS i386 kernel32 fibers',result.stdout)

    def test_memory_and_unicode_edges(self):
        out=ROOT/'build/host/win32/memory-edges';out.mkdir(parents=True,exist_ok=True)
        program=out/'edges'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-DW32_HOST_MODEL',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
            str(ROOT/'tests/host/win32/memory_edges.c'),
            *[str(ROOT/'sdk/win32/kernel32'/n) for n in ('time.c','text.c')],'-o',str(program)],check=True)
        result=subprocess.run([program],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS memory status edge matrix',result.stdout)

    def test_system_power_status(self):
        """G11: GetSystemPowerStatus over the Win32 system action (power_status_test.c)."""
        out=ROOT/'build/host/win32/power-status';out.mkdir(parents=True,exist_ok=True)
        program=out/'status'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-DW32_HOST_MODEL',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
            str(ROOT/'tests/host/win32/power_status_test.c'),
            *[str(ROOT/'sdk/win32/kernel32'/n) for n in ('time.c','text.c')],'-o',str(program)],check=True)
        result=subprocess.run([program],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS GetSystemPowerStatus',result.stdout)

    def test_local_time_views(self):
        """G1: Win32 and DOS views of the system zone agree (localtime_views_test.c)."""
        out=ROOT/'build/host/win32/local-time';out.mkdir(parents=True,exist_ok=True)
        program=out/'views'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-DW32_HOST_MODEL',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
            str(ROOT/'tests/host/win32/localtime_views_test.c'),str(ROOT/'sdk/win32/kernel32/time.c'),
            '-o',str(program)],check=True)
        result=subprocess.run([program],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS local time views',result.stdout)

    def test_file_wrapper_transport(self):
        out=ROOT/'build/host/win32/file-wrappers';out.mkdir(parents=True,exist_ok=True)
        program=out/'file-wrappers'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
            '-DW32_HOST_MODEL','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
            str(ROOT/'tests/host/win32/kernel32_file_wrappers.c'),str(ROOT/'sdk/win32/kernel32/files.c'),
            str(ROOT/'sdk/win32/kernel32/process.c'),str(ROOT/'sdk/win32/kernel32/text.c'),
            '-pthread','-o',str(program)],check=True)
        # ASan's optional fake stack would relocate facade wire pointers above 4 GiB.
        result=subprocess.run([program],capture_output=True,text=True,
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:detect_stack_use_after_return=0'))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS file wrappers',result.stdout)

    def test_offline_facade(self):
        # G26 replaces the former offline facade with a native socket bridge.
        out=ROOT/'build/host/win32/winsock';out.mkdir(parents=True,exist_ok=True)
        program=out/'winsock'
        # Preserve the guest's fixed-width records while transporting actual
        # host pointers through tokens; low-bit reconstruction fails under ASLR.
        source=(ROOT/'sdk/win32/offline/wsock.c').read_text()
        packing='#define WP(p) ((uint32_t)(uintptr_t)(p))'
        receive_length='.address_length=(uint32_t)length'
        self.assertEqual(source.count(packing),1)
        self.assertEqual(source.count(receive_length),1)
        source=source.replace(packing,'#define WP(p) wire_pointer((uintptr_t)(p))')
        source=source.replace(receive_length,'.address_length=sending?(uint32_t)length:WP((void *)length)')
        (out/'wsock_host.inc').write_text(source)
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-DW32_HOST_MODEL',
            '-fno-pie','-no-pie','-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(out),'-I'+str(ROOT/'sdk/win32/offline'),
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
            str(ROOT/'tests/host/win32/offline_test.c'),'-o',str(program)],check=True)
        result=subprocess.run([program],capture_output=True,text=True,
            env=dict(os.environ,ASAN_OPTIONS='detect_stack_use_after_return=0'))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS G26 Winsock',result.stdout)

    def test_profile_two_process_serialization(self):
        out=ROOT/'build/host/win32/profile-processes';out.mkdir(parents=True,exist_ok=True)
        program=out/'processes'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-DW32_HOST_MODEL',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
            str(ROOT/'tests/host/win32/profile_process.c'),
            *[str(ROOT/'sdk/win32/kernel32'/n) for n in ('profile.c','profile_rpc.c')],
            *[str(ROOT/'apps/w32srv'/n) for n in ('registry.c','storage.c','defaults.c','directory.c')],
            '-o',str(program)],check=True)
        result=subprocess.run([program,out/'test.ini'],capture_output=True,text=True,timeout=45,
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS profile processes writers=2 updates=200',result.stdout)

    def test_profile_parity_cases(self):
        out=ROOT/'build/host/win32/profile';out.mkdir(parents=True,exist_ok=True)
        program=out/'profile'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
            '-DW32_HOST_MODEL','-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
            str(ROOT/'tests/host/win32/profile_test.c'),str(ROOT/'sdk/win32/kernel32/profile.c'),'-o',str(program)],check=True)
        result=subprocess.run([program],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS profile first-match',result.stdout)

    def test_drive_service_on_real_vfs(self):
        out=ROOT/'build/host/win32/files';out.mkdir(parents=True,exist_ok=True)
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
        subprocess.run(['python3',str(ROOT/'tests/host/fs/fixtures.py')],check=True,env=env,capture_output=True)
        sources=['tests/host/win32/kernel32_files.c','tests/host/proc/signal_legacy.c','tests/host/proc/desktop_legacy.c',
            'src/kernel/fs/fs_port.c','src/kernel/fs/blkreq.c','src/kernel/fs/cache.c','src/kernel/fs/path.c','src/kernel/fs/fat.c','src/kernel/fs/vfs.c',
            'src/kernel/fs/fs_ro.c','src/kernel/fs/iso9660.c','tests/host/fs/fake.c','src/kernel/probes/f2_probes_files.c']
        program=out/'files'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Wl,--wrap=cache_reserve','-DFS_HOST','-D_POSIX_C_SOURCE=200809L','-pthread',
            '-I'+str(ROOT/'src/kernel/include'),'-I'+str(ROOT/'src/kernel/fs'),*[str(ROOT/s) for s in sources],'-o',str(program)],check=True,env=env)
        result=subprocess.run([program,ROOT/'build/host/fs/fat32.img',ROOT/'build/host/fs/fat16.img'],env=env,capture_output=True,text=True,timeout=60)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS kernel32 production VFS',result.stdout)
    def _host(self,name,sources,marker,extra=(),env=None):
        out=ROOT/'build/host/win32/f5-20a';out.mkdir(parents=True,exist_ok=True)
        program=out/name
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-DW32_HOST_MODEL',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
            *[str(ROOT/s) for s in sources],*extra,'-o',str(program)],check=True)
        result=subprocess.run([program],capture_output=True,text=True,timeout=60,
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:detect_stack_use_after_return=0',**(env or {})))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn(marker,result.stdout)
        return result.stdout

    def test_f5_20a_kernel32_upper_entry_points(self):
        k32='sdk/win32/kernel32/'
        out=self._host('kernel32-upper',['tests/host/win32/kernel32_upper_test.c',
            *[k32+n+'.c' for n in ('wide','paths','locale','message','upper','unsupported','console','text','time')]],
            'PASS kernel32 upper checks=')
        for part in ('Unicode files paths profiles strings objects','SearchPath GetBinaryType ExpandEnvironmentStrings Win16 file API',
                     'NLS LCID names locale info code pages types dates times numbers','FormatMessage strings tables system inserts va_list allocation',
                     'Beep pipes completion ticks work items timers once delay-load atoms time zones',
                     'console log handle queries W21 activation context WOW64 description'):
            self.assertIn('PASS kernel32 upper '+part,out)
        self._host('kernel32-core-upper',['tests/host/win32/kernel32_core_upper.c',k32+'core.c',k32+'wide.c',k32+'text.c'],
            'PASS kernel32 core f5-20A named semaphore event mapping records',extra=('-pthread',))

    def test_f5_20b_unicode_layer_and_collation(self):
        """Directive f5-20 amendment 1 item 1: the former Windows 9x Unicode
        refusals over the ANSI paths, and CompareString/LCMapString A/W."""
        k32='sdk/win32/kernel32/'
        out=self._host('kernel32-unicode',['tests/host/win32/kernel32_unicode_test.c',
            *[k32+n+'.c' for n in ('unicode','collate','locale','wide','text')]],'PASS kernel32 unicode checks=')
        for part in ('files modules objects version environment command line process',
                     'locale info string types','CompareString levels word sort flags A/W',
                     'LCMapString case byte reversal sort keys'):
            self.assertIn('PASS kernel32 unicode '+part,out)

    def test_f5_20a_ntdll_native_registry_strings(self):
        nt='sdk/win32/ntdll/'
        out=self._host('ntdll-upper',['tests/host/win32/ntdll_upper_test.c',
            *[nt+n+'.c' for n in ('native','registry','strings','path','divide')]],'PASS ntdll upper checks=',extra=('-pthread',))
        self.assertIn('PASS ntdll upper NT registry paths access rights dispositions close routing',out)

    def test_f5_15c_ntdll_vsnprintf_wine_semantics(self):
        """f5-15C lead decision: ntdll!_vsnprintf/_vsnwprintf implemented with
        Wine 10.0 dlls/ntdll/printf.h semantics instead of a W21 stub."""
        nt='sdk/win32/ntdll/'
        out=self._host('ntdll-printf',['tests/host/win32/ntdll_printf_test.c',nt+'printf.c',nt+'strings.c'],
                       'PASS ntdll printf checks=')
        self.assertIn('PASS ntdll printf Wine overflow exact-fit counting flags sizes strings pointers',out)

    def test_f5_20a_ntdll_compiler_helpers_i386(self):
        """_alldiv/_allrem/_aulldiv/_chkstk exactly as exported, in a static
        ELF32 that makes no system call (UD2 success, INT3 failure)."""
        out=ROOT/'build/host/win32/f5-20a';out.mkdir(parents=True,exist_ok=True)
        cases=[(-7,2),(7,-2),(-7,-2),(1<<62,3),(-(1<<63),-1),(123456789012345,-1000),(0,5),(-1,(1<<40)+1)]
        def trunc(a,b):
            q=abs(a)//abs(b);q=-q if (a<0)!=(b<0) else q
            return q,a-q*b
        def wrap(v):return v&((1<<64)-1)
        lines=['#include <stdint.h>',
               'extern int64_t __attribute__((stdcall)) _alldiv(int64_t,int64_t);',
               'extern int64_t __attribute__((stdcall)) _allrem(int64_t,int64_t);',
               'extern uint64_t __attribute__((stdcall)) _aulldiv(uint64_t,uint64_t);',
               'int check(void){volatile int64_t a,b;volatile uint64_t u,v;']
        for a,b in cases:
            q,r=trunc(a,b)
            if a==-(1<<63) and b==-1:q,r=a,0
            lines.append(f'a=(int64_t)0x{wrap(a):x}ull;b=(int64_t)0x{wrap(b):x}ull;'
                         f'if((uint64_t)_alldiv(a,b)!=0x{wrap(q):x}ull)return 1;'
                         f'if((uint64_t)_allrem(a,b)!=0x{wrap(r):x}ull)return 2;'
                         f'u=0x{wrap(a):x}ull;v=0x{wrap(b):x}ull;'
                         f'if(_aulldiv(u,v)!=0x{wrap(a)//wrap(b):x}ull)return 3;')
        lines.append('return 0;}')
        driver=out/'helpers.c';driver.write_text('\n'.join(lines)+'\n')
        start=out/'helpers-start.S'
        start.write_text('.text\n.globl _start\n_start:\n movl %esp,%esi\n call check\n cmpl %esp,%esi\n jne 1f\n'
                         ' testl %eax,%eax\n jnz 1f\n movl $8199,%eax\n call __chkstk\n ud2\n1:\n int3\n'
                         '.section .note.GNU-stack,"",@progbits\n')
        objects=[]
        for i,source in enumerate((start,driver,ROOT/'sdk/win32/ntdll/runtime.S',ROOT/'sdk/win32/ntdll/divide.c')):
            obj=out/f'helpers-{i}.o'
            subprocess.run(['clang','--target=i386-unknown-linux-gnu','-march=pentiumpro','-ffreestanding','-fno-builtin',
                '-fno-pic','-O1','-c',str(source),'-o',str(obj)],check=True)
            objects.append(str(obj))
        binary=out/'helpers'
        subprocess.run(['ld.lld','-m','elf_i386','-static','--entry=_start','-o',str(binary),*objects],check=True)
        import resource
        try:
            result=subprocess.run([str(binary)],capture_output=True,timeout=5,
                preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        except OSError as error:
            self.skipTest('host cannot execute ELF32: '+str(error))
        self.assertEqual(result.returncode,-signal.SIGILL,result.stderr)
if __name__=='__main__':unittest.main()
