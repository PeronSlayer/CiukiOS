"""Compile the real shared allocator for isolated VM host fixtures."""
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import runpy
import subprocess
import sys

sys.dont_write_bytecode = True
root, out = map(Path, sys.argv[1:3])
sanitized = '--plain' not in sys.argv[3:]
recipe = runpy.run_path(str(root / 'src/kernel/net/lwip/build.py'))
source = recipe['source']()
out = out / ('pool-asan' if sanitized else 'pool-plain')
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DLWIP_HOST', '-pthread',
         '-ffunction-sections', '-fdata-sections', *recipe['includes'](source),
         '-I', str(root / 'src/kernel/include')]
if sanitized:
    flags += ['-fsanitize=address,undefined']
files = [source / 'src/core' / (n + '.c') for n in ('mem', 'memp', 'pbuf', 'stats')]
files += [root / n for n in ('src/kernel/net/lwip/sys_arch.c', 'src/kernel/net/packet_pool.c',
                            'tests/host/vm/pool_fixture.c')]
objects = []
for i, path in enumerate(files):
    obj = out / (str(i) + '.o')
    subprocess.run(['clang', *flags, '-c', str(path), '-o', str(obj)], check=True)
    objects.append(obj)
library = out / 'pool.a'
subprocess.run(['llvm-ar', 'rcs', str(library), *map(str, objects)], check=True)
print(library)
