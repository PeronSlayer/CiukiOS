#!/usr/bin/env python3
"""Focused production fd/process integration; no filesystem fixture images."""
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import argparse
import subprocess
parser = argparse.ArgumentParser()
parser.add_argument('--frame', action='store_true', help='test production copied NIC bridge instead')
args = parser.parse_args()
ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/lwip'
OUT.mkdir(parents=True, exist_ok=True)
sources = ['tests/host/lwip/fd_test.c', 'tests/host/proc/signal_legacy.c',
           'tests/host/proc/desktop_legacy.c'] + [
    f'src/kernel/fs/{name}.c' for name in ('fs_port', 'blkreq', 'cache', 'path',
                                         'fat', 'vfs', 'fs_ro', 'iso9660')] + [
    'tests/host/fs/fake.c', 'src/kernel/probes/f2_probes_files.c']
if args.frame:
    sources = ['tests/host/lwip/frame_test.c']
binary = OUT / ('frame-test' if args.frame else 'fd-test')
subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
    '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections',
    '-Wl,--gc-sections', '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread',
    '-I', str(ROOT / 'src/kernel/include'), '-I', str(ROOT / 'src/kernel/fs'),
    *[str(ROOT / n) for n in sources], '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True, timeout=30)
