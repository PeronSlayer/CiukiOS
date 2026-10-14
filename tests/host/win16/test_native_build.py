"""Freestanding i686 build of the production parser and a sanitizer mutation run."""
# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from scripts.win16 import fixtures

SOURCE = ROOT/'sdk/win16/ne/ne.c'
# The kernel's freestanding i686 profile (scripts/build_kernel.py), minus the
# stack protector, which needs the kernel's guard symbol.
I686 = ['--target=i686-unknown-elf', '-march=pentiumpro', '-ffreestanding', '-fno-pic', '-fno-pie',
        '-fno-builtin', '-nostdlib', '-mno-sse', '-mno-sse2', '-mno-mmx', '-mno-80387',
        '-mno-red-zone', '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror', '-Wframe-larger-than=4096']


def tool(name):
    path = shutil.which(name)
    if not path:
        raise unittest.SkipTest(name+' is not installed')
    return path


class NativeBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = ROOT/'build/host/win16'
        parent.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='native-', dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.dir = Path(cls.temp.name)

    def test_i686_freestanding_object_has_no_external_references(self):
        obj = self.dir/'ne-i686.o'
        subprocess.run(['nice', '-n', '19', tool('clang'), *I686, '-c', str(SOURCE), '-o', str(obj)],
                       check=True, timeout=120)
        undefined = subprocess.run([tool('llvm-nm'), '-u', str(obj)], check=True, capture_output=True,
                                   text=True, timeout=60).stdout.split()
        self.assertEqual(undefined, [])
        header = subprocess.run([tool('llvm-readelf'), '-h', str(obj)], check=True, capture_output=True,
                                text=True, timeout=60).stdout
        self.assertIn('Intel 80386', header)

    def test_sanitized_parser_loader_relocator_over_fixture_mutations(self):
        exe = self.dir/'ne-mutate'
        subprocess.run(['nice', '-n', '19', tool('clang'), '-std=c11', '-g', '-O1', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer', str(Path(__file__).with_name('ne_mutate.c')),
                        str(SOURCE), '-o', str(exe)], check=True, timeout=180)
        corpus = self.dir/'corpus'
        fixtures.build(corpus)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=0',
                   UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1')
        p = subprocess.run(['nice', '-n', '19', str(exe), '-n', '384', *sorted(map(str, corpus.glob('*.ne')))],
                           capture_output=True, text=True, timeout=600, env=env)
        self.assertEqual(p.returncode, 0, p.stdout+p.stderr)
        self.assertIn(' bad=0', p.stdout)
        counts = dict(field.split('=') for field in p.stdout.split()[1:])
        self.assertGreater(int(counts['accepted']), 0)
        self.assertGreater(int(counts['refused']), 0)
        self.assertGreater(int(counts['relocated']), 0)


if __name__ == '__main__':
    unittest.main()
