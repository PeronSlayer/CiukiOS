#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host"
mkdir -p "$out"
build_test() {
 local binary="$1"; shift
 clang -std=c17 -g -Wall -Wextra -Werror "$@" \
 -ffunction-sections -fdata-sections -Wl,--gc-sections \
 -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -I "$root/src/kernel/include" -I "$root/src/kernel/fs" \
 "$root/tests/host/audio/audio_test.c" "$root/tests/host/proc/signal_legacy.c" \
 "$root/src/kernel/audio/registry_master.c" \
 "$root/tests/host/proc/desktop_legacy.c" "$root/src/kernel/fs/fs_port.c" \
 "$root/src/kernel/fs/cache.c" "$root/src/kernel/fs/path.c" "$root/src/kernel/fs/fat.c" \
 "$root/src/kernel/fs/vfs.c" "$root/src/kernel/fs/fs_ro.c" "$root/src/kernel/fs/iso9660.c" "$root/tests/host/fs/fake.c" \
 "$root/src/kernel/probes/f2_probes_files.c" -o "$binary"
}
build_test "$out/audio_test" -O1 -fsanitize=address,undefined
python3 "$root/tests/host/fs/fixtures.py"
"$out/audio_test" "$out/fs/fat32.img" "$out/fs/fat16.img"
build_test "$out/audio_cost_test" -O2
CIUKI_AUDIO_COST=1 "$out/audio_cost_test" "$out/fs/fat32.img" "$out/fs/fat16.img"
# Inspect the actual kernel target: x86_64 host division timings alone hide
# the i686 runtime-helper overhead that caused the double-refill misses.
clang --target=i686-unknown-elf -march=pentiumpro -O2 -ffreestanding \
 -fno-pic -fno-pie -fno-builtin -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
 -fstack-protector-strong -mstack-protector-guard=global -mstack-alignment=4 \
 -fno-omit-frame-pointer -Wall -Wextra -Werror -Wframe-larger-than=4096 \
 -I "$root/src/kernel/include" -c "$root/src/kernel/audio/mixer.c" -o "$out/audio_mixer_i686.o"
if nm -u "$out/audio_mixer_i686.o" | rg '__[u]?(div|mod)di3'; then
 echo 'FAIL: i686 native mixer still calls wide division helpers' >&2
 exit 1
fi
echo 'audio i686 codegen: no wide division helpers, 4 KiB frame bound PASS'
