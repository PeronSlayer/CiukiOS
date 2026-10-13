#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host/usb"
mkdir -p "$out"
export TMPDIR="$out" PYTHONDONTWRITEBYTECODE=1 ASAN_OPTIONS=detect_leaks=0
python3 "$root/tests/host/fs/fixtures.py"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -DCIUKI_USB_HOST -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$root/src/kernel/include" "$root/tests/host/usb/storage_test.c" \
    "$root/src/kernel/usb/usb_core.c" "$root/src/kernel/usb/usb_storage.c" \
    "$root/src/kernel/fs/fs_port.c" "$root/src/kernel/fs/cache.c" \
    "$root/src/kernel/fs/partition.c" "$root/src/kernel/fs/path.c" \
    "$root/src/kernel/fs/fat.c" "$root/src/kernel/fs/vfs.c" \
    "$root/src/kernel/fs/fs_ro.c" "$root/src/kernel/fs/iso9660.c" \
    "$root/src/kernel/drivers/blkpart.c" "$root/src/kernel/fs/mount.c" \
    -o "$out/storage_test"
"$out/storage_test" "$root/build/host/fs/fat12.img" "$root/build/host/fs/fat16.img" "$root/build/host/fs/fat32.img"
