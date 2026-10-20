"""Desktop f5-09B adapters, copied registry RPC and service lifecycle; no VM."""
from pathlib import Path
import os
import resource
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]
class Win32AdapterTests(unittest.TestCase):
    def test_registry_launch_and_supervision(self):
        work=ROOT/'build/host/desktop-win32';work.mkdir(parents=True,exist_ok=True)
        binary=work/'adapter-test'
        sources=['apps/desktop/win32_registry.c','apps/desktop/win32_launch.c','apps/desktop/win32_service.c',
                 'apps/desktop/protocol.c','tests/host/desktop/win32_adapter_test.c']
        sources += ['apps/w32srv/'+n+'.c' for n in ('registry','storage','directory','defaults')]
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:handle_segv=0',TMPDIR=str(work))
        io_object=work/'posix-io.o'
        subprocess.run(['clang','-std=c17','-O1','-g','-fsanitize=address,undefined',
                        '-I'+str(ROOT/'sdk/win32/include'),'-c',str(ROOT/'apps/w32srv/posix_io.c'),
                        '-o',str(io_object)],check=True,env=env)
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-D_POSIX_C_SOURCE=200809L','-include',str(ROOT/'tests/host/desktop/replay_base.h'),
                        '-fsanitize=address,undefined','-fno-sanitize-recover=undefined',
                        '-I'+str(ROOT/'apps/desktop'),'-I'+str(ROOT/'sdk/win32/include'),
                        '-I'+str(ROOT/'src/kernel/include'),*(str(ROOT/s) for s in sources),
                        str(io_object),'-pthread','-o',str(binary)],check=True,env=env)
        with tempfile.TemporaryDirectory(dir=work) as directory:
            result=subprocess.run([binary,directory],capture_output=True,text=True,env=env)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        (ROOT/'build/codex/f5-09b-adapters.log').write_text(result.stdout+result.stderr)

    def test_real_server_crash_kill_reconnect_and_flush(self):
        work=ROOT/'build/host/desktop-win32';work.mkdir(parents=True,exist_ok=True)
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:handle_segv=0',TMPDIR=str(work))
        common=['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                '-D_POSIX_C_SOURCE=200809L','-fsanitize=address,undefined',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                '-I'+str(ROOT/'apps/desktop'),'-I'+str(ROOT/'sdk/win32/include')]
        io_object=work/'host-server-io.o'
        subprocess.run([*common,'-Dwr_posix_io=host_posix_io','-c',str(ROOT/'apps/w32srv/posix_io.c'),
                        '-Wno-unused-command-line-argument','-o',str(io_object)],check=True,env=env)
        server=work/'real-w32srv'
        server_sources=['apps/w32srv/'+n+'.c' for n in ('registry','storage','directory','defaults','protocol','progman','restart','shell_fs','main')]
        server_sources+=['tests/host/desktop/w32_platform.c','apps/desktop/protocol.c']
        base=['-include',str(ROOT/'tests/host/desktop/replay_base.h'),'-I'+str(ROOT/'src/kernel/include'),
              '-idirafter',str(ROOT/'sdk/sysroot-overlay/include')]
        subprocess.run([*common,*base,'-I'+str(ROOT/'tests/host/desktop/w32-platform'),
                        *(str(ROOT/s) for s in server_sources),str(io_object),'-o',str(server)],check=True,env=env)
        binary=work/'supervision-test'
        sources=['apps/desktop/win32_registry.c','apps/desktop/win32_native.c','apps/desktop/win32_restart.c','apps/desktop/win32_service.c','apps/desktop/protocol.c','apps/desktop/win32_launch.c',
                 'tests/host/desktop/win32_supervision_test.c']
        subprocess.run([*common,*base,*(str(ROOT/s) for s in sources),'-pthread','-o',str(binary)],check=True,env=env)
        with tempfile.TemporaryDirectory(dir=work) as directory:
            result=subprocess.run([binary,server,directory],env=env,capture_output=True,text=True,timeout=30,
                                  preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        (ROOT/'build/codex/f5-09b-supervision.log').write_text(result.stdout+result.stderr)

    def test_production_launcher_arguments_and_directory(self):
        work=ROOT/'build/host/desktop-win32';work.mkdir(parents=True,exist_ok=True)
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',TMPDIR=str(work))
        common=['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-D_POSIX_C_SOURCE=200809L',
                '-include',str(ROOT/'tests/host/desktop/replay_base.h'),'-fsanitize=address,undefined',
                '-I'+str(ROOT/'apps/desktop'),'-I'+str(ROOT/'src/kernel/include'),
                '-I'+str(ROOT/'sdk/win32/include'),'-idirafter',str(ROOT/'sdk/sysroot-overlay/include')]
        obj=work/'launcher.o'
        subprocess.run([*common,'-Dmain=desk_w32_launcher_main','-Dclose=fixture_close','-c',
                        str(ROOT/'apps/desktop/win32_launcher.c'),'-o',str(obj)],check=True,env=env)
        binary=work/'launcher-test'
        subprocess.run([*common,str(obj),str(ROOT/'apps/desktop/win32_launch.c'),
                        str(ROOT/'apps/desktop/protocol.c'),str(ROOT/'tests/host/desktop/win32_launcher_test.c'),
                        '-o',str(binary)],check=True,env=env)
        with tempfile.TemporaryDirectory(dir=work) as directory:
            result=subprocess.run([binary,directory],env=env,capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        (ROOT/'build/codex/f5-09b-launcher.log').write_text(result.stdout+result.stderr)

    def test_native_desktop_service_and_shutdown_wiring(self):
        work=ROOT/'build/host/desktop-win32';work.mkdir(parents=True,exist_ok=True)
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',TMPDIR=str(work))
        binary=work/'native-lifecycle-test'
        sources=['apps/desktop/win32_native.c','apps/desktop/win32_restart.c','apps/desktop/win32_service.c','apps/desktop/win32_launch.c','apps/desktop/protocol.c',
                 'tests/host/desktop/win32_native_lifecycle_test.c']
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-D_POSIX_C_SOURCE=200809L',
                        '-include',str(ROOT/'tests/host/desktop/replay_base.h'),'-fsanitize=address,undefined',
                        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Wl,--wrap=kill',
                        '-I'+str(ROOT/'apps/desktop'),'-I'+str(ROOT/'src/kernel/include'),
                        '-I'+str(ROOT/'sdk/win32/include'),'-idirafter',str(ROOT/'sdk/sysroot-overlay/include'),
                        *(str(ROOT/s) for s in sources),'-pthread','-o',str(binary)],check=True,env=env)
        result=subprocess.run([binary],env=env,capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        (ROOT/'build/codex/f5-09b-native-lifecycle.log').write_text(result.stdout+result.stderr)
