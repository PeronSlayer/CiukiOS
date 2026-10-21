#!/usr/bin/env bash
# Focused production DOS bridge, FAT and VFS host qualification.
set -euo pipefail
cd "$(dirname "$0")/../../.."
mkdir -p build/host/dos
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -fno-omit-frame-pointer -pthread -Isrc/kernel/include -Isrc/kernel/fs \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=aligned_alloc,--wrap=free \
    src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/path.c \
    src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c \
    src/kernel/vm/dos_file.c src/kernel/vm/dos_file_volume.c tests/host/dos_file_api_test.c \
    -o build/host/dos/dos_file_api_test
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    build/host/dos/dos_file_api_test
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    build/host/dos/dos_file_api_test --continuation
# A cancelled f1-84 waiter must neither steal the barrier nor advance its group.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -fno-omit-frame-pointer -pthread -Isrc/kernel/include -Isrc/kernel/fs \
    src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    tests/host/fs/cache_barrier_request_test.c -o build/host/dos/cache_barrier_request_test
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    build/host/dos/cache_barrier_request_test
# Keep H49 itself unchanged; qualify its admission contract together with
# f1-83's ticket retirement and ticket-free per-file read/seek paths.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -fno-omit-frame-pointer -pthread -Isrc/kernel/include -Isrc/kernel/fs \
    src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/fs_ro.c \
    tests/host/fs/test_vfs_admission.c -o build/host/dos/vfs_admission_test
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    build/host/dos/vfs_admission_test
# Compile the changed production units with the real i686 kernel flags. This
# is a compile-only host check: no kernel link, DOS disk or OS image build.
python3 -B - <<'PY'
import importlib.util
from pathlib import Path
import subprocess
spec=importlib.util.spec_from_file_location('kernel_flags','scripts/build_kernel.py')
k=importlib.util.module_from_spec(spec); spec.loader.exec_module(k)
for source in ('vm/dos_file','vm/dos_file_vm','vm/dos_unified','vm/dosvm_bios','vm/dosvm',
               'core/output','probes/f3_probes_dos_conformance'):
    unit=source.rsplit('/',1)[-1]
    subprocess.run(['clang',*k.CFLAGS,'-I','src/kernel/include','-c',f'src/kernel/{source}.c',
                    '-o',f'build/host/dos/{unit}.i686.o'],check=True)
print('H50 i686: changed production units compile with kernel frame/integer flags')
PY
