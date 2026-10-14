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
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -D_POSIX_C_SOURCE=200809L -ffunction-sections -fdata-sections -Wl,--gc-sections \
 -DCIUKI_HOST_OPL3 -I "$root/src/kernel/include" \
 "$root/tests/host/audio/vm_cost_test.c" "$root/src/kernel/audio/opl3.c" -o "$out/vm_arithmetic_test"
"$out/vm_arithmetic_test"
clang -std=c17 -O2 -g -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
 -ffunction-sections -fdata-sections -Wl,--gc-sections -DCIUKI_HOST_OPL3 \
 -I "$root/src/kernel/include" "$root/tests/host/audio/vm_cost_test.c" \
 "$root/src/kernel/audio/opl3.c" -o "$out/vm_cost_test"
"$out/vm_cost_test"
# Inspect the actual kernel target: x86_64 host division timings alone hide
# the i686 runtime-helper overhead that caused the double-refill misses.
target_flags=(--target=i686-unknown-elf -march=pentiumpro -O2 -ffreestanding
 -fno-pic -fno-pie -fno-builtin -mno-sse -mno-sse2 -mno-mmx -mno-80387
 -fstack-protector-strong -mstack-protector-guard=global -mstack-alignment=4
 -fno-omit-frame-pointer -Wall -Wextra -Werror -Wframe-larger-than=4096
 -I "$root/src/kernel/include")
for unit in mixer vm_source opl3; do
 clang "${target_flags[@]}" -c "$root/src/kernel/audio/$unit.c" -o "$out/audio_${unit}_i686.o"
 if nm -u "$out/audio_${unit}_i686.o" | rg '__[u]?(div|mod)di3|__muldi3'; then
  echo "FAIL: i686 $unit calls wide arithmetic helpers" >&2
  exit 1
 fi
done
# Pin the gain operation's actual lowering, including the high half needed
# by +18 dB. Widening two 32-bit operands must be one native IMUL, no helper.
cat > "$out/audio_gain_i686.c" <<'C'
#include "../../src/kernel/audio/vm_source.c"
int32_t audio_codegen_gain(int32_t sample, uint32_t gain)
{ return gained_sample(sample, gain); }
C
clang "${target_flags[@]}" -S "$out/audio_gain_i686.c" -o "$out/audio_gain_i686.s"
# SB16's separate DSP/DMA clock legitimately divides wide timestamps. Guard
# the queue/mixer snapshot entry points used by the refill worker, without
# widening this directive into virtual-hardware timing changes.
clang "${target_flags[@]}" -S "$root/src/kernel/vm/dosvm_sb16.c" -o "$out/audio_sb16_i686.s"
python3 - "$out/audio_sb16_i686.s" "$out/audio_gain_i686.s" <<'PY'
import re, sys
from pathlib import Path
source = Path(sys.argv[1]).read_text()
for name in ('dosvm_audio_info', 'dosvm_audio_fetch', 'dosvm_audio_events', 'dosvm_audio_opl_registers'):
    body = source.split(name + ':', 1)[1].split('.Lfunc_end', 1)[0]
    assert not re.search(r'__(?:u?(?:div|mod)|mul)di3', body), name + ': wide arithmetic helper'
body = Path(sys.argv[2]).read_text().split('audio_codegen_gain:', 1)[1].split('.Lfunc_end', 1)[0]
assert len(re.findall(r'\bimull\b', body)) == 1, 'Q16 gain must use one native signed multiply'
assert not re.search(r'\bcalll?\b', body), 'Q16 gain must not call a helper'
print('audio i686 VM/native/OPL and SB16 render APIs: no wide arithmetic helpers, 4 KiB frame bound PASS')
PY
