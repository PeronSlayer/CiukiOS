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
