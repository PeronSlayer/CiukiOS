"""Sequential host evidence for part A. No image build or guest execution.

UBSan is the default. CIUKI_DISPLAY_ASAN=1 additionally enables ASan with
its default leak detection; no sanitizer option is suppressed here.
"""
from pathlib import Path
import os
import importlib.util
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class DisplayDriverTests(unittest.TestCase):
    def run_host(self, sources, extra=(), wrapper=False, arguments=()):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='display-model-',dir=folder) as tmp:
            tmp = Path(tmp)
            if wrapper:
                source = (ROOT/'sdk/libciuki/posix.c').read_text()
                first = source.index('int ciuki_display_engine_info(')
                last = source.index('\nint ciuki_display_info(',first)
                (tmp/'display_query_wrapper.inc').write_text(source[first:last])
            binary = tmp/'test'
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize='+sanitizer,'-fno-sanitize-recover=undefined',
                '-I',str(ROOT/'src/kernel/include'),'-I',str(tmp),*extra,
                *(str(ROOT/s) for s in sources),'-o',str(binary)],check=True)
            subprocess.run([str(binary),*arguments],check=True)

    def test_abi_oracle(self):
        folder=ROOT/'build/host'; folder.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='display-abi-',dir=folder) as tmp:
            output=Path(tmp)/'target.json'
            subprocess.run(['python3',str(ROOT/'scripts/test/abi_layout_dump.py'),'--output',str(output)],check=True)
            self.run_host(['tests/host/abi_layout_test.c'],arguments=[str(output)])

    def test_target_translation_units(self):
        spec=importlib.util.spec_from_file_location('display_kernel_flags',ROOT/'scripts/build_kernel.py')
        module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        sources=['drivers/video/registry.c','drivers/video/s3savage_register.c','drivers/video/swdib.c','drivers/video/batch.c',
            'proc/display_batch.c','proc/surface.c','proc/grants.c',
            'drivers/fbdev.c','drivers/savage2d.c','core/devices.c','core/display.c','core/hardware_check.c',
            'core/hardware_check_model.c','proc/syscalls_desktop.c']
        folder=ROOT/'build/host'; folder.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='display-target-',dir=folder) as tmp:
            for source in sources:
                subprocess.run(['clang',*module.CFLAGS,'-I',str(ROOT/'src/kernel/include'),
                    '-c',str(ROOT/'src/kernel'/source),'-o',str(Path(tmp)/(source.replace('/','_')+'.o'))],check=True)
        builder=(ROOT/'scripts/build_kernel.py').read_text()
        for name in ('registry.c','s3savage_register.c','swdib.c'):
            self.assertIn('"'+name+'"',builder)

    def test_kernel_syscall(self):
        self.run_host(['tests/host/proc/desktop_test.c','tests/host/proc/signal_legacy.c',
            'src/kernel/lib/sha256.c','src/kernel/lib/fmt.c','src/kernel/probes/selector.c'],
            ['-ffunction-sections','-fdata-sections','-Wl,--gc-sections'])

    def test_software_goldens(self):
        self.run_host(['tests/host/swdib_test.c','src/kernel/drivers/video/swdib.c'])

    def test_registry_integration(self):
        self.run_host(['tests/host/display_driver_test.c'])

    def test_adapter_cursor_evidence(self):
        self.run_host(['tests/host/display_adapter_test.c'],
            ['-ffunction-sections','-fdata-sections','-Wl,--gc-sections'])

    def test_sdk_protocol(self):
        self.run_host(['tests/host/display_query_test.c','apps/desktop/protocol.c'],wrapper=True)

if __name__=='__main__': unittest.main()
