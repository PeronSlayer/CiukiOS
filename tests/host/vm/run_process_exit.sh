#!/usr/bin/env bash
# Real DOS lifecycle cleanup errors and IPX cancellation, host only.
set -euo pipefail
cd "$(dirname "$0")/../../.."
mkdir -p build/host/vm
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -fno-omit-frame-pointer -pthread -Isrc/kernel/include -Isrc/kernel/fs \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=aligned_alloc,--wrap=free \
    src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/path.c \
    src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c \
    src/kernel/vm/dos_file.c src/kernel/vm/dos_file_volume.c src/kernel/vm/dosvm_ipx.c \
    tests/host/vm/process_exit_test.c -o build/host/vm/process_exit_test
build/host/vm/process_exit_test
