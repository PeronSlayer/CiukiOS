#!/usr/bin/env bash
# Focused production DOS bridge, FAT and VFS host qualification.
set -euo pipefail
cd "$(dirname "$0")/../../.."
mkdir -p build/host/dos
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -fno-omit-frame-pointer -pthread -Isrc/kernel/include -Isrc/kernel/fs \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=aligned_alloc,--wrap=free \
    src/kernel/fs/fs_port.c src/kernel/fs/cache.c src/kernel/fs/path.c \
    src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c \
    src/kernel/vm/dos_file.c src/kernel/vm/dos_file_volume.c tests/host/dos_file_api_test.c \
    -o build/host/dos/dos_file_api_test
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    build/host/dos/dos_file_api_test
# Compile the changed production units with the real i686 kernel flags. This
# is a compile-only host check: no kernel link, DOS disk or OS image build.
python3 -B - <<'PY'
import importlib.util
from pathlib import Path
import subprocess
spec=importlib.util.spec_from_file_location('kernel_flags','scripts/build_kernel.py')
k=importlib.util.module_from_spec(spec); spec.loader.exec_module(k)
for unit in ('dos_file','dos_file_vm','dosvm_bios'):
    subprocess.run(['clang',*k.CFLAGS,'-I','src/kernel/include','-c',f'src/kernel/vm/{unit}.c',
                    '-o',f'build/host/dos/{unit}.i686.o'],check=True)
print('H50 i686: changed production units compile with kernel frame/integer flags')
PY
