"""H07: production broker, streaming libreg adapter and owned restart journals."""
import os
from pathlib import Path
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[3]

class Win32BrokerTests(unittest.TestCase):
    def test_broker_and_restart(self):
        out=ROOT/'build/host/security/win32-broker'
        out.mkdir(parents=True,exist_ok=True)
        program=out/'broker'
        env=dict(os.environ,TMPDIR=str(out))
        sources=['tests/host/security/win32_broker_test.c','apps/registry/broker.c','apps/registry/restart.c',
                 'apps/w32srv/registry.c','apps/w32srv/storage.c','apps/w32srv/defaults.c','apps/w32srv/directory.c',
                 'sdk/libreg/registry.c','sdk/libreg/transport.c']
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-DCIUKI_REG_REMOTE','-DCIUKI_REG_TRANSPORT_FAKE','-Wno-misleading-indentation',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'sdk/win32/include'),'-I'+str(ROOT/'src/kernel/include'),
                        *[str(ROOT/s) for s in sources],'-o',str(program)],check=True,env=env)
        result=subprocess.run([str(program)],capture_output=True,text=True,env=env,timeout=120)
        print(result.stdout,end='')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('H07 broker production: PASS',result.stdout)

    def test_kernel_isolation(self):
        out=ROOT/'build/host/security/win32-isolation'
        out.mkdir(parents=True,exist_ok=True)
        program=out/'kernel'
        env=dict(os.environ,TMPDIR=str(out))
        sources=['tests/host/win32/kernel_core_test.c','src/kernel/fs/path.c',
                 'tests/host/proc/signal_legacy.c','tests/host/proc/desktop_legacy.c']
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'src/kernel/include'),
                        *[str(ROOT/s) for s in sources],'-o',str(program)],check=True,env=env)
        result=subprocess.run([str(program),'--security'],capture_output=True,text=True,env=env,timeout=60)
        print(result.stdout,end='')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('H07 kernel:',result.stdout)

    def test_production_registry_adapter(self):
        out=ROOT/'build/host/security/registry-adapter'
        out.mkdir(parents=True,exist_ok=True)
        env=dict(os.environ,TMPDIR=str(out))
        flags=['-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
               '-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all']
        include=['-I'+str(ROOT/'src/kernel/include'),'-I'+str(ROOT/'sdk/win32/include')]
        sources=['apps/registry/broker.c','apps/registry/kernel_adapter.c','apps/registry/server.c',
                 'apps/w32srv/registry.c','apps/w32srv/storage.c','apps/w32srv/defaults.c','apps/w32srv/directory.c',
                 'sdk/libreg/registry.c','sdk/libreg/storage.c','sdk/libreg/transport.c']
        objects=[]
        for name in sources:
            obj=out/(name.replace('/','_')+'.o')
            subprocess.run(['clang',*flags,*include,'-c',str(ROOT/name),'-o',str(obj)],check=True,env=env)
            objects.append(str(obj))
        program=out/'adapter'
        subprocess.run(['clang',*flags,*include,'-Wl,--gc-sections',
                        str(ROOT/'tests/host/security/registry_adapter_test.c'),
                        str(ROOT/'tests/host/proc/signal_legacy.c'),str(ROOT/'src/kernel/lib/sha256.c'),
                        str(ROOT/'src/kernel/lib/tzrule.c'),str(ROOT/'src/kernel/lib/fmt.c'),
                        str(ROOT/'src/kernel/probes/selector.c'),*objects,'-o',str(program)],check=True,env=env)
        result=subprocess.run([str(program)],capture_output=True,text=True,env=env,timeout=60)
        print(result.stdout,end='')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('H07 production adapter:',result.stdout)

    def test_durable_restart_storage_and_quarantine(self):
        out=ROOT/'build/host/security/restart-store'
        out.mkdir(parents=True,exist_ok=True)
        program=out/'store'
        env=dict(os.environ,TMPDIR=str(out))
        sources=['tests/host/security/restart_store_test.c','apps/registry/broker.c','apps/registry/restart.c',
                 'apps/registry/restart_store.c','apps/registry/restart_quarantine.c','apps/registry/restart_posix.c',
                 'apps/w32srv/registry.c','apps/w32srv/storage.c','apps/w32srv/defaults.c','apps/w32srv/directory.c',
                 'sdk/libreg/registry.c','sdk/libreg/storage.c','sdk/libreg/transport.c','src/kernel/lib/sha256.c']
        # Hardware inventory is not used by this private store fixture.
        stub=out/'hardware.c'
        stub.write_text('#include <stdint.h>\nstruct rt; struct rt *reg_hardware(uint64_t p){(void)p;return 0;}\n')
        subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'src/kernel/include'),'-I'+str(ROOT/'sdk/win32/include'),
                        *[str(ROOT/s) for s in sources],str(stub),'-o',str(program)],check=True,env=env)
        result=subprocess.run([str(program)],capture_output=True,text=True,env=env,timeout=120)
        print(result.stdout,end='')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('H07 restart storage production: PASS',result.stdout)
