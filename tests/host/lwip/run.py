#!/usr/bin/env python3
"""Focused real-stack and target compile checks. Run with nice -n 19."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
from pathlib import Path
import runpy
import subprocess
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
recipe = runpy.run_path(str(ROOT / 'src/kernel/net/lwip/build.py'))
source = recipe['source']()
PORT = ROOT / 'src/kernel/net/lwip'
OUT = ROOT / 'build/host/lwip'
OUT.mkdir(parents=True, exist_ok=True)
parser = argparse.ArgumentParser()
parser.add_argument('--kernel', action='store_true')
args = parser.parse_args()
flags = runpy.run_path(str(ROOT / 'scripts/build_kernel.py'))['CFLAGS'] if args.kernel else [
    '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-DLWIP_HOST', '-pthread',
    '-ffunction-sections', '-fdata-sections']
includes = recipe['includes'](source) + ['-I', str(ROOT / 'src/kernel/include')]
if args.kernel:
    includes += ['-I', str(PORT / 'libc')]
files = [source / 'src' / n for n in recipe['SOURCES']] + [PORT / n for n in (
    'sys_arch.c', 'stack.c', 'sockets.c', 'syscalls.c')]
if args.kernel:
    files += [ROOT / n for n in ('src/kernel/net/bridge.c', 'src/kernel/core/syscall.c',
        'src/kernel/proc/fdtable.c', 'src/kernel/proc/channel.c', 'src/kernel/proc/grants.c',
        'src/kernel/proc/surface.c', 'src/kernel/proc/syscalls_file.c',
        'src/kernel/drivers/net.c', 'src/kernel/core/ledger.c')]
objects = []
for index, path in enumerate(files):
    obj = OUT / (('target-' if args.kernel else 'host-') + str(index) + '.o')
    subprocess.run(['clang', *flags, *includes, '-c', str(path), '-o', str(obj)], check=True)
    objects.append(obj)
if args.kernel:
    subprocess.run(['ld.lld', '-r', *map(str, objects), '-o', str(OUT / 'target.o')], check=True)
    print('PASS: production flags,', len(files), 'translation units; relocatable link')
else:
    binary = OUT / 'stack-test'
    subprocess.run(['clang', *flags, *includes, str(ROOT / 'tests/host/lwip/stack_test.c'),
        *map(str, objects), '-Wl,--gc-sections', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=90)
