"""f1-112 production wire/register models, no image or QEMU."""
from pathlib import Path
import argparse
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
SANITIZER = 'address,undefined'

class SerialPeripheralTests(unittest.TestCase):
    def test_input_merge(self):
        out = ROOT / 'build/host/serial-peripheral'
        out.mkdir(parents=True, exist_ok=True)
        binary = out / ('input-' + SANITIZER.replace(',', '-'))
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=' + SANITIZER, '-fno-sanitize-recover=all',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-I', str(ROOT / 'src/kernel/include'),
                        str(ROOT / 'tests/host/serial_input_test.c'), '-o', str(binary)], check=True)
        subprocess.run([binary], check=True)

    def test_wire_and_uart_models(self):
        out = ROOT / 'build/host/serial-peripheral'
        out.mkdir(parents=True, exist_ok=True)
        command = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=' + SANITIZER, '-fno-sanitize-recover=all',
                   '-I', str(ROOT / 'src/kernel/include'),
                   str(ROOT / 'tests/host/serial_peripheral_test.c'),
                   str(ROOT / 'src/kernel/drivers/serial_protocol.c'),
                   str(ROOT / 'src/kernel/vm/uart16550.c'), '-o', str(out / 'test')]
        subprocess.run(command, check=True)
        subprocess.run([out / 'test'], check=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--sanitizer', choices=('address,undefined', 'undefined'), default=SANITIZER)
    args, remaining = parser.parse_known_args()
    SANITIZER = args.sanitizer
    unittest.main(argv=[sys.argv[0], *remaining])
