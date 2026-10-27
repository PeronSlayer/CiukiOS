"""Production COM UART model and native ABI, host-only."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class ComTests(unittest.TestCase):
    def test_uart_model(self):
        parent = ROOT / 'build/host'
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='com-', dir=parent) as temp:
            binary = Path(temp) / 'com'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host/com_test.c'),
                            str(ROOT / 'tests/host/security/subject_fixture.c'),
                            str(ROOT / 'src/kernel/drivers/serial_protocol.c'), '-o', str(binary)], check=True)
            subprocess.run([binary], check=True)

if __name__ == '__main__':
    unittest.main()
