"""H05: production device/admin checks; serial ASan/UBSan with leak checks."""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/security/devices'


class DeviceTests(unittest.TestCase):
    def test_device_enforcement(self):
        OUT.mkdir(parents=True, exist_ok=True)
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/fs/fixtures.py')], check=True)
        sources = ['tests/host/security/devices_test.c', 'tests/host/proc/signal_legacy.c',
                   'tests/host/proc/desktop_legacy.c', 'tests/host/fs/fake.c',
                   'src/kernel/probes/f2_probes_files.c'] + [
            f'src/kernel/fs/{name}.c' for name in ('fs_port', 'blkreq', 'cache', 'path',
                'fat', 'vfs', 'fs_ro', 'iso9660', 'partition', 'disklease')]
        program = OUT / 'devices'
        env = dict(os.environ, TMPDIR=str(OUT))
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
                        '-Wl,--gc-sections', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L',
                        '-pthread', '-I', str(ROOT / 'src/kernel/include'),
                        '-I', str(ROOT / 'src/kernel/fs'),
                        *[str(ROOT / s) for s in sources], '-o', str(program)], check=True, env=env)
        r = subprocess.run([str(program), str(ROOT / 'build/host/fs/fat32.img')],
                           capture_output=True, text=True, env=env, timeout=90)
        print(r.stdout, end='')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('H05 devices: PASS', r.stdout)

    def test_power_enforcement(self):
        OUT.mkdir(parents=True, exist_ok=True)
        program = OUT / 'power'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I', str(ROOT / 'src/kernel/include'),
                        str(ROOT / 'tests/host/power_runtime_test.c'),
                        str(ROOT / 'tests/host/security/subject_fixture.c'),
                        '-o', str(program)], check=True)
        r = subprocess.run([str(program)], capture_output=True, text=True, timeout=90)
        print(r.stdout, end='')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_power_policy(self):
        # sec-k08b: power_policy_test includes subject_fixture.c for session-state cases.
        OUT.mkdir(parents=True, exist_ok=True)
        source = (ROOT / 'src/kernel/core/power.c').read_text()
        a = source.index('int32_t power_record_call(')
        b = source.index('\n#endif', a)
        (OUT / 'power_record.inc').write_text(source[a:b])
        program = OUT / 'power-policy'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I', str(OUT), '-I', str(ROOT / 'src/kernel/include'),
                        str(ROOT / 'tests/host/security/power_policy_test.c'),
                        '-o', str(program)], check=True)
        r = subprocess.run([str(program)], capture_output=True, text=True, timeout=90)
        print(r.stdout, end='')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_input_enforcement(self):
        OUT.mkdir(parents=True, exist_ok=True)
        program = OUT / 'input'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I', str(ROOT / 'src/kernel/include'),
                        str(ROOT / 'tests/host/security/input_devices_test.c'),
                        str(ROOT / 'tests/host/proc/signal_legacy.c'),
                        str(ROOT / 'src/kernel/lib/sha256.c'), str(ROOT / 'src/kernel/lib/fmt.c'),
                        str(ROOT / 'src/kernel/probes/selector.c'), '-o', str(program)], check=True)
        r = subprocess.run([str(program)], capture_output=True, text=True, timeout=90)
        print(r.stdout, end='')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_mixer_enforcement(self):
        OUT.mkdir(parents=True, exist_ok=True)
        host = ROOT / 'build/host'
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/vm_creation_model.py'),
                        str(host / 'vm_creation_model.inc')], check=True)
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/fs/fixtures.py')], check=True)
        program = OUT / 'mixer'
        sources = ['tests/host/audio/audio_test.c', 'tests/host/proc/signal_legacy.c',
                   'tests/host/proc/desktop_legacy.c', 'tests/host/fs/fake.c',
                   'src/kernel/audio/registry_master.c', 'src/kernel/audio/midi.c',
                   'src/kernel/audio/opl3.c', 'src/kernel/lib/sha256.c',
                   'src/kernel/probes/f2_probes_files.c'] + [
            f'src/kernel/fs/{name}.c' for name in ('fs_port', 'blkreq', 'cache', 'path',
                'fat', 'vfs', 'fs_ro', 'iso9660')]
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-fdata-sections',
                        '-Wl,--gc-sections,--wrap=fs_lock_acquired',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread',
                        '-I', str(host), '-I', str(ROOT / 'src/kernel/include'),
                        '-I', str(ROOT / 'src/kernel/fs'),
                        *[str(ROOT / n) for n in sources], '-o', str(program)], check=True)
        r = subprocess.run([str(program), str(host / 'fs/fat32.img'), str(host / 'fs/fat16.img')],
                           env=dict(os.environ, CIUKI_AUDIO_SECURITY='1'),
                           capture_output=True, text=True, timeout=90)
        print(r.stdout, end='')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_com_enforcement(self):
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/test_com.py')], check=True)

    def test_lpt_enforcement(self):
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/test_lpt.py'),
                        'LptTests.test_register_model'], check=True)

    def test_network_enforcement(self):
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/lwip/run.py'), '--netctl'], check=True)


if __name__ == '__main__':
    unittest.main()
