#!/usr/bin/env bash
# Production persistent DOS data tests, ASan/UBSan and default leak detection.
set -euo pipefail
cd "$(dirname "$0")/../../.."
mkdir -p build/host/dos
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -fno-omit-frame-pointer -pthread -Isrc/kernel/include -Isrc/kernel/fs \
    src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c \
    src/kernel/fs/partition.c src/kernel/drivers/blkpart.c src/kernel/lib/sha256.c \
    src/kernel/vm/dos_save.c src/kernel/vm/dos_save_store.c src/kernel/vm/dosvm_saves.c src/kernel/vm/dos_file_volume.c src/kernel/vm/dos_identity.c \
    src/kernel/lib/fmt.c \
    tests/host/dos/persistent_saves_test.c -o build/host/dos/persistent_saves_test
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    build/host/dos/persistent_saves_test
