"""Kernel32 file endpoint over the production filesystem, without QEMU."""
import os
import signal
from pathlib import Path
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[3]
class Kernel32Files(unittest.TestCase):
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
            *[str(ROOT/'sdk/win32/kernel32'/n) for n in ('time.c','text.c','unicode.c')],'-o',str(program)],check=True)
        result=subprocess.run([program],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS memory status edge matrix',result.stdout)

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
        out=ROOT/'build/host/win32/offline';out.mkdir(parents=True,exist_ok=True)
        program=out/'offline'
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-DW32_HOST_MODEL',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'sdk/win32/include'),
            str(ROOT/'tests/host/win32/offline_test.c'),str(ROOT/'sdk/win32/offline/wsock.c'),'-o',str(program)],check=True)
        result=subprocess.run([program],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS offline Winsock',result.stdout)

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
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DFS_HOST','-D_POSIX_C_SOURCE=200809L','-pthread',
            '-I'+str(ROOT/'src/kernel/include'),'-I'+str(ROOT/'src/kernel/fs'),*[str(ROOT/s) for s in sources],'-o',str(program)],check=True,env=env)
        result=subprocess.run([program,ROOT/'build/host/fs/fat32.img',ROOT/'build/host/fs/fat16.img'],env=env,capture_output=True,text=True,timeout=60)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS kernel32 production VFS',result.stdout)
if __name__=='__main__':unittest.main()
