#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host"
mkdir -p "$out"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -ffunction-sections -fdata-sections -Wl,--gc-sections \
 -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -I "$root/src/kernel/include" -I "$root/src/kernel/fs" \
 "$root/tests/host/audio/audio_test.c" "$root/tests/host/proc/signal_legacy.c" \
 "$root/tests/host/proc/desktop_legacy.c" "$root/src/kernel/fs/fs_port.c" \
 "$root/src/kernel/fs/cache.c" "$root/src/kernel/fs/path.c" "$root/src/kernel/fs/fat.c" \
 "$root/src/kernel/fs/vfs.c" "$root/src/kernel/fs/fs_ro.c" "$root/src/kernel/fs/iso9660.c" "$root/tests/host/fs/fake.c" \
 "$root/src/kernel/probes/f2_probes_files.c" -o "$out/audio_test"
python3 "$root/tests/host/fs/fixtures.py"
"$out/audio_test" "$out/fs/fat32.img" "$out/fs/fat16.img"
