"""Production UART initialization, IRQ transmitter and formatted records."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SerialTests(unittest.TestCase):
    def run_fixture(self, name):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='serial-', dir=folder) as temp:
            binary = Path(temp) / name
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-pthread', '-ffunction-sections',
                            '-fdata-sections', '-Wl,--gc-sections',
                            '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host' / (name + '.c')),
                            str(ROOT / 'src/kernel/lib/fmt.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_probe(self):
        self.run_fixture('serial_probe_test')

    def test_irq_transmitter(self):
        self.run_fixture('serial_audio_test')

    def test_records(self):
        self.run_fixture('serial_record_test')
