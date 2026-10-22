#!/usr/bin/env bash
# Production V86/DPMI monitor plus production DOS file owner, host CPU only.
set -euo pipefail
cd "$(dirname "$0")/../../.."
mkdir -p build/host/dos build/codex
if [[ ! -f /usr/include/x86emu.h ]]; then
    echo 'H50 CPU: unavailable (libx86emu development headers missing)'
    exit 1
fi
python3 -B tests/host/dos/hook_cpu.py --prepare
clang -DFS_HOST -DCIUKI_DOSVM_FILE_CPU -D_POSIX_C_SOURCE=200809L \
    -std=c17 -O1 -g -Wall -Wextra -Werror -Wno-pointer-to-int-cast \
    -fsanitize=address,undefined -pthread \
    -Isrc/kernel/include -Ibuild/host tests/host/dosvm_guest.c \
    tests/host/dosvm_guest_cpu.c src/kernel/lib/fmt.c src/kernel/lib/sha256.c \
    src/kernel/vm/virtual_vga.c src/kernel/vm/virtual_vga_bios.c \
    src/kernel/vm/vga_x86.c src/kernel/vm/vga_presenter.c \
    src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/path.c \
    src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c \
    src/kernel/fs/partition.c src/kernel/drivers/blkpart.c \
    src/kernel/vm/dos_file.c src/kernel/vm/dos_file_volume.c src/kernel/vm/dos_unified.c \
    src/kernel/vm/dos_unified_ifs.c \
    -lx86emu -o build/host/dos/dosvm_hook_cpu
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 python3 -B tests/host/dos/hook_cpu.py
