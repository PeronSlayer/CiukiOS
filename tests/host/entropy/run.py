#!/usr/bin/env python3
"""Entropy host fixtures or production-flag compile checks; no image build."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
from pathlib import Path
import runpy
import subprocess
import sys
sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[3]
out = root / 'build/host/entropy'
out.mkdir(parents=True, exist_ok=True)
parser = argparse.ArgumentParser()
parser.add_argument('--kernel', action='store_true')
parser.add_argument('--case', choices=('model', 'kernel', 'lwip-seed'))
args = parser.parse_args()
includes = ['-I', str(root / 'src/kernel/include')]
if args.kernel:
    flags = runpy.run_path(str(root / 'scripts/build_kernel.py'))['CFLAGS']
    objects = []
    for name in ('lib/entropy_pool', 'lib/sha256', 'lib/stackprot', 'core/entropy',
                 'core/syscall', 'core/init', 'arch/trap', 'proc/syscalls_entropy'):
        obj = out / (name.replace('/', '-') + '.o')
        subprocess.run(['clang', *flags, *includes, '-c', str(root / f'src/kernel/{name}.c'), '-o', str(obj)], check=True)
        objects.append(str(obj))
    subprocess.run(['ld.lld', '-r', *objects, '-o', str(out / 'target.o')], check=True)
    runpy.run_path(str(root / 'scripts/build_kernel.py'))['audit'](out / 'target.o')
    print('PASS entropy production flags: eight translation units, relocatable link and instruction audit')
else:
    flags = ['-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    common = [str(root / f'src/kernel/lib/{n}.c') for n in ('entropy_pool', 'sha256')]
    for name in ((args.case,) if args.case else ('model', 'kernel', 'lwip-seed')):
        binary = out / (name + '-test')
        extra, sources = [], common
        if name == 'lwip-seed':
            recipe = runpy.run_path(str(root / 'src/kernel/net/lwip/build.py'))
            extra = ['-DLWIP_HOST', '-pthread', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                     *recipe['includes'](recipe['source']())]
            sources = [str(root / 'src/kernel/net/lwip/stack.c')]
        source_name = name.replace('-', '_')
        subprocess.run(['clang', *flags, *includes, *extra, str(root / f'tests/host/entropy/{source_name}_test.c'),
                        *sources, '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=60)
